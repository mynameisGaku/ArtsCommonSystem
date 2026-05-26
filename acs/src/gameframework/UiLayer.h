// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — UiLayer (acs::ui Widget tree を Scene に繋ぐ glue)
//
// 役割:
//   既存の `acs::ui` Widget システム (Widget.h / Widgets.h / UiRenderer.h) は
//   retained-mode UI として強力だが、ゲームコード側 (Scene) から直接触るには
//   ボイラープレートが多い。UiLayer は **Scene のライフサイクル (Init / Tick /
//   HandleInput / Shutdown) と Widget tree を結ぶ薄い層** で、典型的なゲーム UI
//   (ボタン / テキスト表示) を最小 API で扱えるようにする。
//
//   Pillar H 上のリッチな UI 構築 (StackPanel / Slider / TextInput etc.) は
//   引き続き `acs::ui` を直接使う想定で、本 layer は **「シーンに数個の UI
//   要素を貼り付けるだけのゲーム」** の DX (タイトル画面、HUD、ポーズメニュー
//   など) を改善する位置付け。
//
// 使い方 (典型例):
//   class TitleScene : public Scene {
//       UiLayer _ui;
//       u32     _play_btn = 0;
//       void OnEnter() noexcept override {
//           _ui.Init();
//           _play_btn = _ui.AddButton("Play", {100, 200}, {200, 48});
//           _ui.AddText("ACS Demo", {100, 100});
//       }
//       void OnUpdate(f32 dt) noexcept override {
//           _ui.Tick(dt);
//           if (_ui.IsButtonPressed(_play_btn)) {
//               Scenes().ChangeScene(MakeUnique<GameScene>());
//           }
//       }
//       void OnEvent(const Event& e) noexcept override { _ui.HandleInput(e); }
//       void OnExit() noexcept override { _ui.Shutdown(); }
//   };
//
// 設計選択 (Phase H-1 スケルトン):
//   ・**state holder のみ実装**: 実 Widget 生成 / 描画 / ヒットテストは現フェーズ
//     では未接続 (Init / Tick / HandleInput 内に TODO コメントで明示)。代わりに
//     ハンドル付きの WidgetEntry を TArray で保持し、追加・削除・可視性・押下
//     クエリは完全動作する。これにより呼び出し側 (Scene) は本 layer を通常通り
//     使い始めることができ、Phase H-2 で `acs::ui::StackPanel` 等の実 widget
//     接続に差し替えるだけで描画 / 入力が動く設計。
//   ・**ハンドルは u32 単調増加**: 削除後の再利用は行わない (世代カウンタ不要、
//     現実的に 1 シーンで数千 widget を超えることはまずないため uint32 で十分)。
//     0 は invalid handle 予約。
//   ・**const char* 非所有**: 規約通り <string> 不使用。label / text の寿命は
//     呼び出し側 (文字列リテラル or 長寿命バッファ) が保証する。Pillar T
//     PartySystem / Pillar O Entitlement と同方針。
//   ・**コピー / ムーブ禁止**: UI 状態を持つ長寿命オブジェクト。誤コピーで state
//     が分裂すると詰むため非コピー・非ムーブ。
//   ・**全 noexcept**: ACS 全体方針。Init / Shutdown は冪等で再呼び出し安全。
//
// 範囲外 (Phase H-2 以降で接続):
//   ・実 `acs::ui::Widget` ツリー構築 (StackPanel / Button / TextBlock の生成)
//   ・`UiRenderer` による描画 (RenderContext から SpriteBatch / Font を受ける)
//   ・`Event` → hit-test → Widget::OnPointerDown 等の配送
//   ・focus / keyboard navigation (Tab 移動 / Enter 確定)
//   ・MVVM Observable<T> との bind (現状はポーリング型 IsButtonPressed)
//   ・slider / checkbox / text input / radio などの追加 widget kind
//   ・layout 自動配置 (現状は呼び出し側が pos / size を絶対指定)
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs {

// Scene.h と同様、`Event` は `acs` 名前空間直下に forward declaration する
// (実体は `platform/Event.h`)。HandleInput の引数で参照を取るためだけに必要。
struct Event;

namespace game {

// UiLayer が扱う widget の種類。Phase H-1 はボタンとテキストのみ。Phase H-2 で
// Slider / Checkbox / TextInput を追加する想定。
enum class EWidgetKind : u8 {
    None    = 0,
    Button  = 1,  // クリックで押下イベントを発火する短形ボタン
    Text    = 2,  // 単純な静的テキスト (押下ヒットテストなし)
};

// 単一 widget の状態 1 件分。state holder の中核。
//   ・handle       : AddButton / AddText が返す不透明 ID (0 は invalid)
//   ・kind         : 種別 (Button / Text)。判別に必要 (描画 / 入力分岐)
//   ・pos, size    : 画面ピクセル単位の絶対座標 / サイズ。Text は size 未使用
//   ・text         : ラベル / 表示テキスト (非所有、寿命は呼び出し側保証)
//   ・visible      : 不可視時は描画もヒットテストもスキップ
//   ・just_pressed : 直前フレームで押された (Tick 開始時に clear、次フレームで
//                    呼び出し側が IsButtonPressed で読み取って消費する想定)
struct WidgetEntry {
    u32         handle       = 0;
    EWidgetKind  kind         = EWidgetKind::None;
    acs::FVec2   pos         {0.0f, 0.0f};
    acs::FVec2   size        {0.0f, 0.0f};
    const char* text         = nullptr;
    bool        visible      = true;
    bool        just_pressed = false;
};

class UiLayer {
public:
    UiLayer()  noexcept = default;
    ~UiLayer() noexcept = default;

    // 非コピー・非ムーブ (state holder。誤コピーで widget 状態が分裂しないよう)。
    UiLayer(const UiLayer&)            = delete;
    UiLayer& operator=(const UiLayer&) = delete;
    UiLayer(UiLayer&&)                 = delete;
    UiLayer& operator=(UiLayer&&)      = delete;

    // ----- ライフサイクル -----
    // Scene::OnEnter から呼ぶ。Widget tree の root を確保 (現フェーズは stub、
    // Phase H-2 で `acs::ui::Container` を root として new する)。冪等で再呼出
    // 安全 (二度 Init してもメモリリークしない)。
    void Init() noexcept;

    // Scene::OnExit から呼ぶ。全 widget をクリアし、root を解放。冪等。
    void Shutdown() noexcept;

    // Scene::OnUpdate から呼ぶ。Phase H-1 では `just_pressed` フラグの伝搬
    // ハンドリングのみ。Phase H-2 で `acs::ui::Widget::Layout` 再計算と
    // tween / animation の更新を入れる。
    void Tick(f32 dt) noexcept;

    // Scene::OnEvent から呼ぶ。マウス / キーイベントを widget に dispatch
    // する。Phase H-1 は TODO (HitTestRecursive 接続待ち)。
    void HandleInput(const acs::Event& event) noexcept;

    // 現在登録されている widget 数 (visible / hidden 問わず)。テスト / デバッグ向け。
    u32 WidgetCount() const noexcept;

    // ----- 簡素 widget API -----
    // ボタンを追加。戻り値は 0 でない handle (IsButtonPressed / SetVisible /
    // Remove に渡す)。label は非所有 (寿命は呼び出し側保証)。
    u32 AddButton(const char* label, acs::FVec2 pos, acs::FVec2 size) noexcept;

    // 静的テキストを追加。戻り値は handle。text は非所有。size は描画時に
    // フォントメトリックから自動計算する想定 (Phase H-2)。
    u32 AddText(const char* text, acs::FVec2 pos) noexcept;

    // ボタンが「直前フレームで押されたか」を返す。Phase H-1 では HandleInput
    // で押下検出が未実装なため常に false (state holder のみ)。Phase H-2 で
    // hit-test と event 配送が繋がると正常動作する。invalid handle / Text
    // kind には常に false。
    bool IsButtonPressed(u32 handle) const noexcept;

    // 可視性を変更。invalid handle は無視 (ログ警告のみ)。
    void SetVisible(u32 handle, bool visible) noexcept;

    // 指定 handle の widget を削除。invalid handle は無視。削除後の handle は
    // 再利用されない (世代カウンタなし)。
    void Remove(u32 handle) noexcept;

    // 全 widget を削除。Init 後の初期化やシーン内画面切替で利用。Shutdown とは
    // 異なり root は保持されるので、続けて AddButton 等が可能。
    void Clear() noexcept;

private:
    // 内部探索ヘルパ。指定 handle のインデックスを返す。見つからない / handle == 0
    // なら -1 相当 (u32 の MAX = 0xFFFFFFFFu) を返す。
    u32 FindIndex(u32 handle) const noexcept;

    TArray<WidgetEntry> _widgets;          // 全 widget の状態 (handle 順は保証しない)
    u32                _next_handle = 1;  // 次に発行する handle (0 は invalid 予約)
    bool               _initialized = false;
};

} // namespace game
} // namespace acs
