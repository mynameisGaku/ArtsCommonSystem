// SPDX-License-Identifier: Apache-2.0
// FUiRenderer — Widget tree を FSpriteBatch + Font で描画する
//
// 使い方:
//   FUiRenderer ur;
//   ur.Init(*dev, GetRenderer().ColorFormat(), default_font);
//
//   // 毎フレーム:
//   StackPanel root;
//   /* root.Add<...>() で子を構築 */
//   root.Layout(0, 0, screen_w, screen_h);
//   ur.Render(root, *cmd, screen_w, screen_h);
//
// 仕組み:
//   ・FSpriteBatch で矩形 + テクスチャ + 文字を発行
//   ・Font は ACS Font (TTrueType + atlas)
//   ・Widget::Render(*this) を再帰的に呼ぶ
//   ・各 widget は描画ヘルパ (DrawRect / DrawText 等) で FUiRenderer に依頼
#pragma once

#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"
#include "ui/Widgets.h"

namespace acs {

/**
 * Widget ツリーを FSpriteBatch + Font で描画する UI レンダラ。
 *
 * @details
 * Render で root の Layout → SpriteBatch::Begin → root.Render(*this) → End を行い、
 * 各 widget は DrawRect / DrawRectOutline / DrawText で描画を依頼する。テーマ色 FUiColors を
 * 保持する。Font は所有せず参照のみ。非コピー。
 */
class FUiRenderer {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FUiRenderer() noexcept = default;

    /** 破棄する (SpriteBatch のリソースは Shutdown / デストラクタで解放)。 */
    ~FUiRenderer() noexcept = default;

    /** コピー禁止 (SpriteBatch を単独所有するため)。 */
    FUiRenderer(const FUiRenderer&) = delete;

    /** コピー代入も禁止。 */
    FUiRenderer& operator=(const FUiRenderer&) = delete;

    /**
     * SpriteBatch を初期化し、既定フォントを設定する。
     *
     * @param device パイプライン生成に使う RHI デバイス。
     * @param rt_format 描画先レンダーターゲットのフォーマット。
     * @param default_font 文字描画に使う既定フォント (所有しない、null 可)。
     * @return 成功なら空の TResult、SpriteBatch 初期化失敗ならエラー。
     */
    TResult<void> Init(IRhiDevice& device, EFormat rt_format, Font* default_font) noexcept;

    /** GPU リソースを解放しフォント参照を切る。 */
    void Shutdown() noexcept;

    /**
     * Widget ツリーを 1 フレーム分レイアウトして描画する。
     *
     * @details root を画面全体に Layout し、SpriteBatch を Begin/End で囲んで再帰描画する。
     * @param root 描画するツリーの root widget。
     * @param cmd コマンドを積むコマンドリスト。
     * @param screen_w 画面幅 (px)。
     * @param screen_h 画面高さ (px)。
     */
    void Render(Widget& root, IRhiCommandList& cmd, u32 screen_w, u32 screen_h) noexcept;

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
     * テーマ色への const 参照を返す (Widget の Render が色を参照)。
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
    Font* DefaultFont() const noexcept { return m_Font; }

private:
    /** 矩形・文字を発行するスプライトバッチ。 */
    FSpriteBatch m_Batch;

    /** 文字描画に使う既定フォント (所有しない)。 */
    Font*       m_Font = nullptr;

    /** ウィジェット描画に使うテーマ色。 */
    FUiColors    m_Colors;

    /** Begin と End の間 (描画発行可能) かを示すフラグ。 */
    bool        m_bFrameOpen = false;
};

/**
 * マウス/キー入力を Widget ツリーに配信する入力ディスパッチャ。
 *
 * @details
 * 毎フレーム Dispatch を呼び、Input モジュールのマウス位置・クリック・押下キーを読み取って
 * root を hit-test し、該当 widget の On* イベントに振り分ける。hover / pressed / focused の
 * 対象 widget を内部に保持して状態遷移を管理する。
 */
class FUiInput {
public:
    /**
     * 入力を読み取り、ツリーをヒットテストして該当 widget にイベントを配信する。
     *
     * @param root イベント配信対象のツリーの root widget。
     */
    void Dispatch(Widget& root) noexcept;

private:
    /** 直近に hover 中の widget (所有しない)。 */
    Widget* m_Hovered  = nullptr;

    /** pointer-down 中で drag を受け続ける widget (所有しない)。 */
    Widget* m_Pressed  = nullptr;

    /** 入力フォーカス中の widget (TextInput 等、所有しない)。 */
    Widget* m_Focused  = nullptr;
};

} // namespace acs
