// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar — SceneInspector / AEditorToolbar 実装
//
// 設計のポイント (詳細はヘッダ参照):
//   ・Playing / Paused / Stepping の 3 状態を持ち、ボタン操作 ⇄ CGame::TimeScale を
//     双方向に橋渡しする。
//   ・Stepping は「1 fixed step 進めたら自動で Paused に戻る」一時状態。
//     DrawUI 末尾で TimeScale=0 に戻すことで、ホストの CGame ループは 1 フレーム
//     ぶん scaled dt = (fixed_dt × 1.0) で update した後、次フレームから再び
//     dt=0 になる ≒ "1 step だけ進めた" 挙動を実現する。
//   ・ImGui 依存は本 .cpp に閉じる (ヘッダから漏らさない)。

#include "gameframework/tools/inspector/EditorToolbar.h"

#include "gameframework/Game.h"

#include <imgui.h>

namespace acs::game::inspector {

/** 状態を Playing に戻して初期化する (Save callback は維持してセッション再開に使える)。 */
void AEditorToolbar::Init() noexcept {
    // 状態は Playing に戻す。Save callback は意図的にクリアしない (Init を
    // 「セッション再開」用途で呼ぶこともあるため。完全初期化したい場合は
    // Shutdown → Init の順で呼ぶ)。
    _state              = EEditorState::Playing;
    m_NormalTimeScale  = 1.0f;
    m_ShowDebugOverlay = false;
}

/** 状態・callback・フラグを全てデフォルトに戻して後始末する。 */
void AEditorToolbar::Shutdown() noexcept {
    // 完全初期化: 状態 + callback + flag を全てデフォルトに。
    _state              = EEditorState::Playing;
    m_NormalTimeScale  = 1.0f;
    m_ShowDebugOverlay = false;
    m_SaveCb            = nullptr;
    m_SaveUser          = nullptr;
}

/**
 * Play / Pause を切り替える。
 *
 * @details
 * Playing → Paused、Paused → Playing。Stepping 中の呼び出しは明示 Play 要求とみなして
 * Step を中断し Playing に復帰する。実際の TimeScale 反映は DrawUI の ApplyStateToGame が行う。
 */
void AEditorToolbar::TogglePlayPause() noexcept {
    switch (_state) {
        case EEditorState::Playing:
            // 現 TimeScale を記録するのは DrawUI 側 (CGame への参照が無いと
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

/**
 * 1 フレームだけ進めて自動的に Paused に戻る Stepping 状態へ遷移する。
 *
 * @details
 * 既に Stepping 中の再 Step は no-op。Playing 中でも許容し、1 step 進めた後に Paused へ戻す
 * (= Pause + Step の合成)。Paused 復帰は DrawUI 末尾で行う。
 */
void AEditorToolbar::Step() noexcept {
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

/**
 * Save Scene の callback を登録する。
 *
 * @param cb 呼ぶ callback (nullptr で解除)。
 * @param user callback に渡すユーザポインタ。
 */
void AEditorToolbar::SetOnSaveSceneCallback(SaveSceneCallback cb, void* user) noexcept {
    m_SaveCb   = cb;
    m_SaveUser = user;
}

/**
 * 現在の state を CGame の TimeScale に反映する (DrawUI から呼ぶ)。
 *
 * @details
 * Playing → TimeScale = m_NormalTimeScale、Paused → TimeScale = 0 (直前の正の TimeScale を
 * 復帰値として記録)、Stepping → TimeScale = m_NormalTimeScale を 1 フレームだけ設定する
 * (Paused 復帰は DrawUI 末尾、実 TimeScale=0 は次フレームの本関数が担当)。
 * @param game TimeScale を書き換える対象の CGame。
 */
void AEditorToolbar::ApplyStateToGame(CGame& game) noexcept {
    switch (_state) {
        case EEditorState::Playing: {
            // Pause から復帰した直後はここに来る。
            // 記憶していた m_NormalTimeScale を書き戻す。
            game.SetTimeScale(m_NormalTimeScale);
            break;
        }
        case EEditorState::Paused: {
            // 直近の TimeScale を記録 (= 後で Play に戻る時の復帰値)。
            // ただし 0 (= 既に Pause 済) は無視 (= 二重 Pause で
            // m_NormalTimeScale を 0 に上書きしないため)。
            const f32 ts = game.TimeScale();
            if (ts > 0.0f) {
                m_NormalTimeScale = ts;
            }
            game.SetTimeScale(0.0f);
            break;
        }
        case EEditorState::Stepping: {
            // 1 フレームだけ通常速度で進める。Stepping → Paused 復帰は
            // DrawUI 末尾で行うため、ここでは scale を m_NormalTimeScale に
            // 設定するだけ。
            game.SetTimeScale(m_NormalTimeScale);
            break;
        }
    }
}

/**
 * ツールバーを 1 つの独立 ImGui window として描画する。
 *
 * @details
 * 横長の button row [ Play ][ Pause ][ Step ] | [ Save ] | [v] CDebugOverlay | state を出す。
 * BeginMainMenuBar はエディタアプリ側で使われている可能性があるため、専用 window で
 * レイアウト衝突を避ける。ボタン操作の結果は末尾の ApplyStateToGame で CGame に反映し、
 * Stepping は同フレームで Paused へ戻す。CGame 未設定 (nullptr) のときは TimeScale 反映を skip する。
 */
void AEditorToolbar::DrawUI() noexcept {
    // AEditorPanel 継承で no-param 化。CGame は SetGame で事前 set。
    // nullptr のときは UI のみ描画して time scale 反映を skip。
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

    // Play / Pause / Step ボタン行。
    // 現状の state でハイライトする (= 現在押されているように見せる)。
    // ImGui::Button は戻り値で「クリックされたか」を返す。
    if (ImGui::Button("Play")) {
        // Pause / Stepping から Playing へ。同じ Playing で押された場合は
        // m_NormalTimeScale をそのまま再適用する (= no-op に近い)。
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

    // Save Scene。
    // callback 未登録時は disabled 風表示 (AParticleEditorPanel と同じパターン)。
    const bool save_enabled = (m_SaveCb != nullptr);
    if (!save_enabled) ImGui::BeginDisabled();
    if (ImGui::Button("Save")) {
        if (m_SaveCb != nullptr) {
            m_SaveCb(m_SaveUser);
        }
    }
    if (!save_enabled) ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // デバッグオーバーレイ表示を切り替える。
    // bool flag を ImGui::Checkbox で切り替えるだけ。実描画は外側の責務。
    ImGui::Checkbox("Debug Overlay", &m_ShowDebugOverlay);

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // 現状表示 (デバッグ用)。
    const char* state_label = "Playing";
    switch (_state) {
        case EEditorState::Playing:  state_label = "Playing";  break;
        case EEditorState::Paused:   state_label = "Paused";   break;
        case EEditorState::Stepping: state_label = "Stepping"; break;
    }
    ImGui::Text("state: %s  (scale=%.2f)", state_label,
                static_cast<double>(m_NormalTimeScale));

    // state を CGame に反映。
    // ボタン操作の結果を本フレームの末尾で 1 度だけ反映する。
    // (ボタン処理ごとに SetTimeScale を呼ぶと、同フレーム内で複数回
    //  TimeScale が動くのを避けるため最後にまとめる。)
    if (m_Game != nullptr) {
        ApplyStateToGame(*m_Game);
    }

    // Stepping の自動 Pause 復帰。
    // Stepping は「本フレーム 1 回だけ Playing 相当の dt を走らせる」一時状態。
    // 本フレームの SetTimeScale = m_NormalTimeScale は既に発行済 (= CGame の
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
