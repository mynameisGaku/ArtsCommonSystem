// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs {

// Scene.h と同様、`FEvent` は `acs` 名前空間直下に forward declaration する
// (実体は `platform/Event.h`)。HandleInput の引数で参照を取るためだけに必要。
struct FEvent;

namespace game {

class FRenderContext;

/**
 * CUiLayer が扱う widget の種類。
 *
 * @details 現状はボタンとテキストのみ (ASlider / ACheckbox / ATextInput は範囲外)。
 */
enum class EWidgetKind : u8 {
    /** 未設定 / 無効。 */
    None    = 0,

    /** クリックで押下イベントを発火する矩形ボタン。 */
    Button  = 1,

    /** 単純な静的テキスト (押下ヒットテストなし)。 */
    Text    = 2,
};

/**
 * 単一 widget の状態 1 件分 (state holder の中核)。
 */
struct FWidgetEntry {
    /** AddButton / AddText が返す不透明 ID (0 は invalid)。 */
    u32         handle       = 0;

    /** widget の種別 (描画 / 入力分岐に使う)。 */
    EWidgetKind  kind         = EWidgetKind::None;

    /** 画面ピクセル単位の絶対座標。 */
    acs::FVec2   pos         {0.0f, 0.0f};

    /** 画面ピクセル単位のサイズ (Text は未使用)。 */
    acs::FVec2   size        {0.0f, 0.0f};

    /** ラベル / 表示テキスト (非所有、寿命は呼び出し側保証)。 */
    const char* text         = nullptr;

    /** 可視フラグ (不可視時は描画もヒットテストもスキップ)。 */
    bool        visible      = true;

    /** 直前フレームで押されたフラグ (consume-on-read。Tick 開始時に clear)。 */
    bool        just_pressed = false;

    /** カーソルが上に乗っているフラグ (描画ハイライト用)。 */
    bool        hovered      = false;

    /** pointer-down 中フラグ (押し込み描画用)。 */
    bool        pressed_down = false;
};

/**
 * acs::ui の AWidget tree を AScene のライフサイクルに繋ぐ薄い glue 層。
 *
 * @details
 * AScene の Init / Tick / HandleInput / Shutdown と AWidget tree を結び、典型的なゲーム UI
 * (ボタン / テキスト表示) を最小 API で扱えるようにする。現状は実 AWidget 生成 / 描画 /
 * ヒットテストは未接続の state holder で、ハンドル付きの FWidgetEntry を TArray で保持し、
 * 追加・削除・可視性・押下クエリは完全動作する。ハンドルは u32 単調増加で再利用しない
 * (0 は invalid 予約)。const char* は非所有 (寿命は呼び出し側保証)。非コピー・非ムーブ、
 * 全 noexcept、Init / Shutdown は冪等。
 */
class CUiLayer {
public:
    /** 空の UI レイヤを構築する (widget 未登録、未初期化)。 */
    CUiLayer()  noexcept = default;

    /** 破棄する。 */
    ~CUiLayer() noexcept = default;

    /** コピー禁止 (state holder。誤コピーで widget 状態が分裂しないため)。 */
    CUiLayer(const CUiLayer&)            = delete;

    /** コピー代入も禁止。 */
    CUiLayer& operator=(const CUiLayer&) = delete;

    /** ムーブ禁止 (state holder。誤コピーで widget 状態が分裂しないため)。 */
    CUiLayer(CUiLayer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    CUiLayer& operator=(CUiLayer&&)      = delete;

    /**
     * UI レイヤを初期化する (AScene::OnEnter から呼ぶ)。
     *
     * @details
     * AWidget tree の root を確保する (現状は stub、実装時は acs::AContainer を root として
     * new する)。冪等で再呼び出し安全 (二度 Init してもメモリリークしない)。
     */
    void Init() noexcept;

    /** 全 widget をクリアし root を解放する (AScene::OnExit から呼ぶ。冪等)。 */
    void Shutdown() noexcept;

    /**
     * UI 状態を更新する (AScene::OnUpdate から呼ぶ)。
     *
     * @details
     * 現状は just_pressed フラグの伝搬ハンドリングのみ。実装時に acs::AWidget::Layout
     * 再計算と tween / animation の更新を入れる。
     * @param dt 前フレームからの経過秒。
     */
    void Tick(f32 dt) noexcept;

    /**
     * マウスイベントを処理する (AScene::OnEvent から呼ぶ)。
     *
     * @details
     * MouseMoved はカーソル位置を記録して hover 状態を更新、MouseButtonPressed は
     * カーソル位置の最前面ボタンを押し込み状態に、MouseButtonReleased は押し込み開始ボタンと
     * 同じボタン上で離したら「クリック成立」= just_pressed を立てる。
     * @param event 処理するプラットフォームイベント。
     */
    void HandleInput(const acs::FEvent& event) noexcept;

    /**
     * 登録 widget を描画する (AScene::OnDrawHud から呼ぶ)。
     *
     * @details
     * rc の CSpriteBatch + FFont で描画する (ボタンは hover/押下で色変化、Text はラベル)。
     * CSpriteBatch セッションが開いている前提で、FFont が無い環境では矩形のみ描画する。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void Draw(FRenderContext& rc) const noexcept;

    /**
     * 現在登録されている widget 数を返す (visible / hidden 問わず)。
     *
     * @return 登録 widget 数 (テスト / デバッグ向け)。
     */
    u32 WidgetCount() const noexcept;

    /**
     * ボタンを追加する。
     *
     * @param label ボタンのラベル (非所有、寿命は呼び出し側保証)。
     * @param pos 画面ピクセル単位の絶対座標。
     * @param size 画面ピクセル単位のサイズ。
     * @return 0 でない handle (IsButtonPressed / SetVisible / Remove に渡す)。
     */
    u32 AddButton(const char* label, acs::FVec2 pos, acs::FVec2 size) noexcept;

    /**
     * 静的テキストを追加する。
     *
     * @details size は描画時にフォントメトリックから自動計算する想定。
     * @param text 表示テキスト (非所有、寿命は呼び出し側保証)。
     * @param pos 画面ピクセル単位の絶対座標。
     * @return 発行した handle。
     */
    u32 AddText(const char* text, acs::FVec2 pos) noexcept;

    /**
     * ボタンが直前フレームで押されたかを返す。
     *
     * @param handle 確認するボタンの handle。
     * @return 押されていれば true。invalid handle / Text kind には常に false。
     */
    bool IsButtonPressed(u32 handle) const noexcept;

    /**
     * widget の可視性を変更する。
     *
     * @param handle 対象 widget の handle (invalid handle はログ警告のみで無視)。
     * @param visible 可視にするなら true。
     */
    void SetVisible(u32 handle, bool visible) noexcept;

    /**
     * 指定 handle の widget を削除する。
     *
     * @details 削除後の handle は再利用されない (世代カウンタなし)。
     * @param handle 削除する widget の handle (invalid handle は無視)。
     */
    void Remove(u32 handle) noexcept;

    /**
     * 全 widget を削除する。
     *
     * @details
     * Init 後の初期化やシーン内画面切替で利用する。Shutdown とは異なり root は保持されるので、
     * 続けて AddButton 等が可能。
     */
    void Clear() noexcept;

private:
    /**
     * 指定 handle の widget のインデックスを返す内部探索ヘルパ。
     *
     * @param handle 探す widget の handle。
     * @return 見つかったインデックス。見つからない / handle == 0 なら 0xFFFFFFFFu。
     */
    u32 FindIndex(u32 handle) const noexcept;

    /**
     * (x,y) を含む最前面の visible なボタンの handle を返す。
     *
     * @param x カーソルの X 座標 (画面ピクセル)。
     * @param y カーソルの Y 座標 (画面ピクセル)。
     * @return 最後に追加された (= 最前面) ヒットボタンの handle。無ければ 0。
     */
    u32 HitTopButton(f32 x, f32 y) const noexcept;

    /** 全 widget の状態 (handle 順は保証しない)。 */
    TArray<FWidgetEntry> m_Widgets;

    /** 次に発行する handle (0 は invalid 予約)。 */
    u32                m_NextHandle  = 1;

    /** 直近のカーソル X 座標 (MouseMoved で更新)。 */
    f32                m_MouseX      = 0.0f;

    /** 直近のカーソル Y 座標 (MouseMoved で更新)。 */
    f32                m_MouseY      = 0.0f;

    /** pointer-down を開始したボタンの handle (0 = 無し)。 */
    u32                m_PressedHandle = 0;

    /** Init 済みフラグ。 */
    bool               m_Initialized = false;
};

/** 旧名を使う既存コード向けの一時的な互換別名。 */
using FUiLayer = CUiLayer;

} // namespace game
} // namespace acs
