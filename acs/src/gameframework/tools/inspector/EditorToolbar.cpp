// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — SceneInspector / EditorToolbar 実装 (Phase 20)
//
// 設計のポイント (詳細はヘッダ参照):
//   ・Playing / Paused / Stepping の 3 状態を持ち、ボタン操作 ⇄ Game::TimeScale を
//     双方向に橋渡しする。
//   ・Stepping は「1 fixed step 進めたら自動で Paused に戻る」一時状態。
//     DrawUI 末尾で TimeScale=0 に戻すことで、ホストの Game ループは 1 フレーム
//     ぶん scaled dt = (fixed_dt × 1.0) で update した後、次フレームから再び
//     dt=0 になる ≒ "1 step だけ進めた" 挙動を実現する。
//   ・ImGui 依存は本 .cpp に閉じる (ヘッダから漏らさない)。

#include "gameframework/tools/inspector/EditorToolbar.h"

#include "gameframework/Game.h"

#include <imgui.h>

namespace acs::game::inspector {

// ============================================================================
// Init / Shutdown
// ============================================================================
void EditorToolbar::Init() noexcept {
    // 状態は Playing に戻す。Save callback は意図的にクリアしない (Init を
    // 「セッション再開」用途で呼ぶこともあるため。完全初期化したい場合は
    // Shutdown → Init の順で呼ぶ)。
    _state              = EEditorState::Playing;
    _normal_time_scale  = 1.0f;
    _show_debug_overlay = false;
}

void EditorToolbar::Shutdown() noexcept {
    // 完全初期化: 状態 + callback + flag を全てデフォルトに。
    _state              = EEditorState::Playing;
    _normal_time_scale  = 1.0f;
    _show_debug_overlay = false;
    _save_cb            = nullptr;
    _save_user          = nullptr;
}

// ============================================================================
// 状態遷移 API
// ============================================================================
void EditorToolbar::TogglePlayPause() noexcept {
    switch (_state) {
        case EEditorState::Playing:
            // 現 TimeScale を記録するのは DrawUI 側 (Game への参照が無いと
            // 取れないため)。ここでは状態だけ Paused へ遷移。
            _state = EEditorState::Paused;
            break;
        case EEditorState::Paused:
            _state = EEditorState::Playing;
            break;
        case EEditorState::Stepping:
            // Step 中の Toggle は「Playing 復帰」とみなす (Stepping は短命
            // な一時状態なので、明示 Play 要求が来たら Step を中断して Play)。
            _state = EEditorState::Playing;
            break;
    }
}

void EditorToolbar::Step() noexcept {
    // 既に Stepping 中の再 Step は no-op (連続 Step は呼び出し側で間に
    // DrawUI を挟むことで実現する)。
    if (_state == EEditorState::Stepping) {
        return;
    }
    // 通常は Paused からの Step が想定される (= 1 フレーム進めて Paused 復帰)。
    // Playing 中の Step は実用上意味が薄いが、安全のため許容: Stepping に
    // 遷移して 1 step 進めた後 Paused に戻る (= Pause + Step の合成)。
    _state = EEditorState::Stepping;
}

void EditorToolbar::SetOnSaveSceneCallback(SaveSceneCallback cb, void* user) noexcept {
    _save_cb   = cb;
    _save_user = user;
}

// ============================================================================
// 内部: state → Game への反映
// ============================================================================
// DrawUI から呼ぶ。state に応じて Game::SetTimeScale を更新する。
//   Playing  → TimeScale = _normal_time_scale
//   Paused   → TimeScale = 0
//   Stepping → TimeScale = _normal_time_scale (1 フレームだけ。DrawUI 末尾で
//              Paused に戻し、次フレームの DrawUI で 0 になる)
// ============================================================================
void EditorToolbar::ApplyStateToGame(Game& game) noexcept {
    switch (_state) {
        case EEditorState::Playing: {
            // Pause から復帰した直後はここに来る。
            // 記憶していた _normal_time_scale を書き戻す。
            game.SetTimeScale(_normal_time_scale);
            break;
        }
        case EEditorState::Paused: {
            // 直近の TimeScale を記録 (= 後で Play に戻る時の復帰値)。
            // ただし 0 (= 既に Pause 済) は無視 (= 二重 Pause で
            // _normal_time_scale を 0 に上書きしないため)。
            const f32 ts = game.TimeScale();
            if (ts > 0.0f) {
                _normal_time_scale = ts;
            }
            game.SetTimeScale(0.0f);
            break;
        }
        case EEditorState::Stepping: {
            // 1 フレームだけ通常速度で進める。Stepping → Paused 復帰は
            // DrawUI 末尾で行うため、ここでは scale を _normal_time_scale に
            // 設定するだけ。
            game.SetTimeScale(_normal_time_scale);
            break;
        }
    }
}

// ============================================================================
// DrawUI
// ============================================================================
// レイアウト (横長 button row、main menu bar の下に配置想定):
//   [ Play ] [ Pause ] [ Step ] | [ Save ] | [v] DebugOverlay  | state: Playing
//
// ImGui のレイアウトは BeginMainMenuBar ではなく独立 Window として実装する。
// (BeginMainMenuBar は editor アプリ側で既に使われている可能性があるため、
//  toolbar 専用 window でレイアウト衝突を避ける。)
// ============================================================================
void EditorToolbar::DrawUI(Game& game) noexcept {
    // フラグ: タイトルバー / リサイズ無し、main viewport 上端に固定する
    // 想定だが、初回 ImGui Init では既定位置に置き、ユーザが動かせるよう
    // collapsible だけ無効化する保守的な設定。
    constexpr ImGuiWindowFlags kFlags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (!ImGui::Begin("Editor Toolbar", nullptr, kFlags)) {
        ImGui::End();
        return;
    }

    // ----- Play / Pause / Step ボタン行 -----
    // 現状の state でハイライトする (= 現在押されているように見せる)。
    // ImGui::Button は戻り値で「クリックされたか」を返す。
    if (ImGui::Button("Play")) {
        // Pause / Stepping から Playing へ。同じ Playing で押された場合は
        // _normal_time_scale をそのまま再適用する (= no-op に近い)。
        _state = EEditorState::Playing;
    }
    ImGui::SameLine();

    if (ImGui::Button("Pause")) {
        // 任意 state から Paused へ強制遷移。
        _state = EEditorState::Paused;
    }
    ImGui::SameLine();

    // Step は Paused のときだけ意味があるが、Playing でも押せるようにして
    // 「現フレームで進めて止める」操作も許容する (Step の意味論はヘッダ参照)。
    if (ImGui::Button("Step")) {
        Step();
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ----- Save Scene -----
    // callback 未登録時は disabled 風表示 (ParticleEditorPanel と同じパターン)。
    const bool save_enabled = (_save_cb != nullptr);
    if (!save_enabled) ImGui::BeginDisabled();
    if (ImGui::Button("Save")) {
        if (_save_cb != nullptr) {
            _save_cb(_save_user);
        }
    }
    if (!save_enabled) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ----- DebugOverlay 切替 -----
    // bool flag を ImGui::Checkbox で切り替えるだけ。実描画は外側の責務。
    ImGui::Checkbox("DebugOverlay", &_show_debug_overlay);

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ----- 現状表示 (デバッグ用) -----
    const char* state_label = "Playing";
    switch (_state) {
        case EEditorState::Playing:  state_label = "Playing";  break;
        case EEditorState::Paused:   state_label = "Paused";   break;
        case EEditorState::Stepping: state_label = "Stepping"; break;
    }
    ImGui::Text("state: %s  (scale=%.2f)", state_label,
                static_cast<double>(_normal_time_scale));

    // ----- state を Game に反映 -----
    // ボタン操作の結果を本フレームの末尾で 1 度だけ反映する。
    // (ボタン処理ごとに SetTimeScale を呼ぶと、同フレーム内で複数回
    //  TimeScale が動くのを避けるため最後にまとめる。)
    ApplyStateToGame(game);

    // ----- Stepping の自動 Pause 復帰 -----
    // Stepping は「本フレーム 1 回だけ Playing 相当の dt を走らせる」一時状態。
    // 本フレームの SetTimeScale = _normal_time_scale は既に発行済 (= Game の
    // 次回 Update では dt が scale 1 で 1 回流れる)、次フレーム以降は再び
    // Paused に戻したいので、ここで state だけを切り替える。
    // 注意: 実 SetTimeScale=0 は次フレームの DrawUI で ApplyStateToGame が
    //   担当する (今フレームの SetTimeScale を上書きしないため)。
    if (_state == EEditorState::Stepping) {
        _state = EEditorState::Paused;
    }

    ImGui::End();
}

} // namespace acs::game::inspector
