// SPDX-License-Identifier: Apache-2.0
// CUiRenderer — AWidget tree を CSpriteBatch + FFont で描画する
//
// 使い方:
//   CUiRenderer ur;
//   ur.Init(*dev, GetRenderer().ColorFormat(), default_font);
//
//   // 毎フレーム:
//   AStackPanel root;
//   /* root.Add<...>() で子を構築 */
//   ur.Render(root, *cmd, screen_w, screen_h);
//
// 仕組み:
//   ・CSpriteBatch で矩形 + テクスチャ + 文字を発行
//   ・FFont は ACS FFont (TTrueType + atlas)
//   ・AWidget::Render(*this) を再帰的に呼ぶ
//   ・各 widget は描画ヘルパ (DrawRect / DrawText 等) で CUiRenderer に依頼
//   ・root.visible=false のフレームは Layout と描画をともに省略
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"
#include "ui/Widgets.h"

namespace acs {

namespace ui_detail {

// テキスト入力欄の選択ハイライト矩形を計算する。
// 左右 6 px、上下 3 px の余白を適用し、入力欄の内容領域へ切り詰める。
// 描画可能な領域がない場合は幅または高さが 0 の矩形を返す。
FUiRect ComputeTextSelectionHighlightRect(const FUiRect& input_rect,
                                           f32 prefix_start,
                                           f32 prefix_end) noexcept;


/**
 * visible な root だけを Layout し、その後の描画処理を呼ぶ共通 gate。
 *
 * @details CUiRenderer::Render と GPU-free test が同じ可視性分岐を共有する。
 * root が hidden なら Layout も callback も呼ばない。
 */
template<typename TRenderCallback>
bool VisitVisibleUiRoot(AWidget& root, f32 width, f32 height,
                        TRenderCallback&& render_callback) noexcept {
    if (!root.visible) return false;
    root.Layout(0.0f, 0.0f, width, height);
    render_callback();
    return true;
}

} // namespace ui_detail

/**
 * AWidget ツリーを CSpriteBatch + FFont で描画する UI レンダラ。
 *
 * @details
 * Render で visible な root の Layout → CSpriteBatch::Begin → root.Render(*this) → End を行い、
 * 各 widget は DrawRect / DrawRectOutline / DrawText で描画を依頼する。テーマ色 FUiColors を
 * 保持する。FFont は所有せず参照のみ。非コピー。
 */
class CUiRenderer {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    CUiRenderer() noexcept = default;

    /** 破棄する (CSpriteBatch のリソースは Shutdown / デストラクタで解放)。 */
    ~CUiRenderer() noexcept = default;

    /** コピー禁止 (CSpriteBatch を単独所有するため)。 */
    CUiRenderer(const CUiRenderer&) = delete;

    /** コピー代入も禁止。 */
    CUiRenderer& operator=(const CUiRenderer&) = delete;

    /**
     * CSpriteBatch を初期化し、既定フォントを設定する。
     *
     * @param device パイプライン生成に使う RHI デバイス。
     * @param rt_format 描画先レンダーターゲットのフォーマット。
     * @param default_font 文字描画に使う既定フォント (所有しない、null 可)。
     * @return 成功なら空の TResult、CSpriteBatch 初期化失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, EFormat rt_format, FFont* default_font) noexcept;

    /** GPU リソースを解放しフォント参照を切る。 */
    void Shutdown() noexcept;

    /**
     * AWidget ツリーを 1 フレーム分レイアウトして描画する。
     *
     * @details visible な root を画面全体に Layout し、CSpriteBatch を Begin/End で囲んで
     * 再帰描画する。root.visible が false なら Layout と全描画を省略する。
     * @param root 描画するツリーの root widget。
     * @param cmd コマンドを積むコマンドリスト。
     * @param screen_w 画面幅 (px)。
     * @param screen_h 画面高さ (px)。
     */
    void Render(AWidget& root, IRhiCommandList& cmd, u32 screen_w, u32 screen_h) noexcept;

    /**
     * 塗りつぶし矩形を発行する (widget の Render から呼ぶ)。
     *
     * @param x 左上 X 座標 (px)。
     * @param y 左上 Y 座標 (px)。
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     * @param color 塗り色 (RGBA)。
     */
    void DrawRect(f32 x, f32 y, f32 w, f32 h, const FVec4& color) noexcept;

    /**
     * 矩形の枠線を発行する (4 辺を太さ thickness の矩形で描く)。
     *
     * @param x 左上 X 座標 (px)。
     * @param y 左上 Y 座標 (px)。
     * @param w 幅 (px)。
     * @param h 高さ (px)。
     * @param color 枠の色 (RGBA)。
     * @param thickness 枠線の太さ (px、既定 1)。
     */
    void DrawRectOutline(f32 x, f32 y, f32 w, f32 h, const FVec4& color, f32 thickness = 1.0f) noexcept;

    /**
     * UTF-8 文字列を既定フォントで描画する。
     *
     * @param utf8 描画する UTF-8 文字列。
     * @param x 描画開始 X 座標 (px)。
     * @param y 描画開始 Y 座標 (px)。
     * @param color 文字色 (RGBA)。
     */
    void DrawText(const char* utf8, f32 x, f32 y, const FVec4& color) noexcept;

    /**
     * 既定フォントで UTF-8 文字列の描画幅 (px) を測る。
     *
     * @details caret 位置決め等に使う。フォント未設定 / 文字列 null なら 0 を返す。
     * @param utf8 測定する UTF-8 文字列。
     * @return 文字列の描画幅 (px)。
     */
    f32 MeasureText(const char* utf8) const noexcept;

    /**
     * UTF-8 文字列の先頭 byte_count バイトだけを既定フォントで測る。
     *
     * @details NUL 終端の一時コピーを確保しない。byte_count が文字列終端より長い場合は
     * NUL までを測り、不正 UTF-8 列は 1 byte ずつ安全に読み飛ばす。
     * @param utf8 NUL 終端 UTF-8 文字列。
     * @param byte_count 測定対象とする最大バイト数。
     * @return 対象 prefix の描画幅 (px)。
     */
    f32 MeasureTextBytes(const char* utf8, usize byte_count) const noexcept;

    /**
     * テーマ色への const 参照を返す (AWidget の Render が色を参照)。
     *
     * @return 現在の FUiColors への const 参照。
     */
    const FUiColors& Colors() const noexcept { return m_Colors; }

    /**
     * テーマ色への可変参照を返す (色を上書きする用)。
     *
     * @return 現在の FUiColors への参照。
     */
    FUiColors&       Colors()       noexcept { return m_Colors; }

    /**
     * 既定フォントを返す。
     *
     * @return 設定済みの既定フォント (未設定なら nullptr、所有しない)。
     */
    FFont* DefaultFont() const noexcept { return m_Font; }

private:
    /** 矩形・文字を発行するスプライトバッチ。 */
    CSpriteBatch m_Batch;

    /** 文字描画に使う既定フォント (所有しない)。 */
    FFont*       m_Font = nullptr;

    /** ウィジェット描画に使うテーマ色。 */
    FUiColors    m_Colors;

    /** Begin と End の間 (描画発行可能) かを示すフラグ。 */
    bool        m_bFrameOpen = false;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FUiRenderer = CUiRenderer;

/**
 * マウス/キー入力を AWidget ツリーに配信する入力ディスパッチャ。
 *
 * @details
 * 毎フレーム Dispatch を呼び、Input モジュールのマウス位置・クリック・キー押下/解放を読み取って
 * root を hit-test し、該当 widget の On* イベントに振り分ける。hover / pressed / focused の
 * 対象は生ポインタではなく widget の address token + 構築 module token + generation で
 * 追跡し、毎回現在の root から解決する。
 * このため root の差し替え、アドレス再利用、同じ root 内の child 除去後にも解放済み
 * widget を参照しない。キー配信時は左右の Shift / Ctrl / Alt / Super を
 * FUiKeyModifiers にまとめ、押下と解放の両方を修飾キー対応の OnKey overload へ渡す。
 *
 * @warning Dispatch の呼び出し中は root 自身を生存させること。callback から UI を更新する
 * 場合も、実行中の Dispatch が戻る前に root 自身を破棄してはならない。
 * DLL unload/reload では module address 自体が再利用され得るため、host は root destruction /
 * module unload 境界で必ず Reset(live_root) または Reset() を呼び、古い追跡を破棄すること。
 */
class CUiInput {
public:
    CUiInput() noexcept = default;
    ~CUiInput() noexcept = default;

    /** 入力追跡 identity を複製して別 dispatcher から再利用しない。 */
    CUiInput(const CUiInput&) = delete;
    CUiInput& operator=(const CUiInput&) = delete;
    CUiInput(CUiInput&&) = delete;
    CUiInput& operator=(CUiInput&&) = delete;

    /**
     * 入力を読み取り、ツリーをヒットテストして該当 widget にイベントを配信する。
     *
     * @param root イベント配信対象のツリーの root widget。
     */
    void Dispatch(AWidget& root) noexcept;

    /**
     * 保存中の root / hover / pressed / focus / Ctrl+A 状態を破棄する。
     *
     * @details 保存状態は整数の複合 identity だけなので、この overload は widget を
     * 一切参照しない。
     * 現在の root を破棄する直前や、その寿命が既に不明な場合にも安全に呼べる。
     * 同じ生存 root を再び Dispatch すると、最初に subtree の一時入力フラグを初期化する。
     * DLL unload/reload や module 所有 root の破棄境界では host が必ず呼ぶ。
     */
    void Reset() noexcept;

    /**
     * 生存中の root に残る一時入力フラグも解除して保存状態を破棄する。
     *
     * @details scene 切り替え等で古い root を保持・再利用する場合に使う。
     * @param live_root 現在生存している root。Dispatch 中の root と異なっていても安全。
     */
    void Reset(AWidget& live_root) noexcept;

private:
    using FTrackedIdentity = ui_detail::FWidgetInputIdentity;

    /** root が変わった場合に追跡を破棄し、新しい subtree を安全な初期状態へする。 */
    void PrepareRoot(AWidget& root) noexcept;

    /** 現在の生存 subtree から可視な追跡対象を解決する。 */
    AWidget* ResolveVisible(AWidget& root,
                            const FTrackedIdentity& identity) noexcept;

    /** 除去・非表示になった追跡対象と対応フラグを破棄する。 */
    void ValidateTrackedState(AWidget& root) noexcept;

    /** 現在 Dispatch 対象の root 複合 identity (全フィールド 0 は未設定)。 */
    FTrackedIdentity m_RootIdentity;

    /** 直近に hover 中の widget 複合 identity。 */
    FTrackedIdentity m_HoveredIdentity;

    /** pointer-down 中で drag を受け続ける widget 複合 identity。 */
    FTrackedIdentity m_PressedIdentity;

    /** 入力フォーカス中の widget 複合 identity。 */
    FTrackedIdentity m_FocusedIdentity;

    /** Ctrl+A 押下を受け、対応する A 解放を待つ widget 複合 identity。 */
    FTrackedIdentity m_ControlAOwnerIdentity;
};

/** 旧名を使う既存コード向けの互換別名。 */
using FUiInput = CUiInput;


} // namespace acs
