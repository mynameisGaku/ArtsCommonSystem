// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "container/Array.h"
#include "container/String.h"
#include "math/Vec.h"

namespace acs::game {

/**
 * atlas 内の 1 矩形 = 1 frame。
 *
 * @details
 * name は caller 所有 (文字列リテラル前提)。pivot は frame のローカル空間 [0,1]
 * (0,0=左上 / 1,1=右下) で表し、描画時のアンカー (回転中心 / 配置基準) に使う。
 */
struct FSpriteFrame {
    /** 検索キー (caller 所有、文字列リテラル想定)。 */
    const char* name    = nullptr;

    /** atlas 内 X 座標 (pixel)。 */
    u32         x       = 0;

    /** atlas 内 Y 座標 (pixel)。 */
    u32         y       = 0;

    /** 矩形の幅 (pixel)。 */
    u32         w       = 0;

    /** 矩形の高さ (pixel)。 */
    u32         h       = 0;

    /** pivot の X (frame ローカル [0,1]、既定は中心)。 */
    f32         pivot_x = 0.5f;

    /** pivot の Y (frame ローカル [0,1]、既定は中心)。 */
    f32         pivot_y = 0.5f;
};

/**
 * atlas 全体のメタ情報。
 *
 * @details texture そのものは別モジュール (CAssetBundle 等) が所有する。
 */
struct FSpritePackInfo {
    /** atlas テクスチャのパス (caller 所有)。 */
    const char* atlas_texture_path = nullptr;

    /** atlas テクスチャの幅 (pixel、0 = 未設定)。 */
    u32         atlas_width        = 0;

    /** atlas テクスチャの高さ (pixel、0 = 未設定)。 */
    u32         atlas_height       = 0;
};

/** 検証付き atlas JSON loader が返す安定した失敗理由。 */
enum class ESpritePackLoadError : u16 {
    /** atlas JSON の読み込みが成功した。 */
    None = 0,
    /** 入力 JSON pointer が null だった。 */
    NullInput = 1450,
    /** 入力 JSON が空だった。 */
    EmptyInput = 1451,
    /** 入力 JSON が読み込み上限を超えた。 */
    InputTooLarge = 1452,
    /** 入力 JSON 内に埋め込み NUL があった。 */
    EmbeddedNul = 1453,
    /** JSON の入れ子が安全上限を超えた。 */
    JsonDepthExceeded = 1454,
    /** JSON string のバイト数が上限を超えた。 */
    JsonStringTooLong = 1455,
    /** JSON node 件数が安全上限を超えた。 */
    JsonNodeLimitExceeded = 1456,
    /** JSON の構文を解釈できなかった。 */
    JsonSyntaxError = 1457,
    /** JSON root が要求された object ではなかった。 */
    RootTypeMismatch = 1458,
    /** 同じ JSON member が複数回現れた。 */
    DuplicateMember = 1459,
    /** 必須 JSON member がなかった。 */
    MissingMember = 1460,
    /** JSON member の型が atlas 契約と一致しなかった。 */
    MemberTypeMismatch = 1461,
    /** 整数 member を範囲内の整数として解釈できなかった。 */
    InvalidInteger = 1462,
    /** 数値 member が有限値ではなかった。 */
    NonFiniteNumber = 1463,
    /** frame 件数が安全上限を超えた。 */
    FrameLimitExceeded = 1464,
    /** frame 名のバイト数が上限を超えた。 */
    NameTooLong = 1465,
    /** 同じ frame 名が複数回現れた。 */
    DuplicateFrameName = 1466,
    /** atlas の幅または高さが有効範囲外だった。 */
    InvalidAtlasSize = 1467,
    /** frame rectangle が atlas 範囲内に収まらなかった。 */
    InvalidFrameRect = 1468,
    /** frame pivot が許可範囲外だった。 */
    InvalidPivot = 1469,
    /** atlas image path のバイト数が上限を超えた。 */
    ImagePathTooLong = 1470,
    /** frame table 用メモリを確保できなかった。 */
    AllocationFailure = 1471,
};

/** TryLoadAtlasJson が返す allocation-free の結果。 */
struct FSpritePackLoadResult {
    ESpritePackLoadError Error = ESpritePackLoadError::None;
    u16 JsonSubcode = 0u;
    u32 Frame = 0u;

    bool Succeeded() const noexcept {
        return Error == ESpritePackLoadError::None;
    }
    explicit operator bool() const noexcept { return Succeeded(); }
};

/** ESpritePackLoadError に対応する安定した診断名。 */
const char* SpritePackLoadErrorName(ESpritePackLoadError error) noexcept;

/**
 * 1 枚の atlas テクスチャと名前付き frame 矩形群を持つデータ層。
 *
 * @details
 * 描画 API・asset loader には触れず「矩形と名前の辞書」に徹する。利用者は
 * AtlasTexturePath() で texture を自前ロードし、FindFrame() で矩形と pivot を取り出し、
 * ComputeUv() で [0,1] UV を得て CSpriteAnimator と組み合わせて再生する。frame 名は
 * const char* 借用 (caller 所有) で、比較は pointer 同一 → strcmp の順に評価する。
 */
class FSpritePack {
public:
    static constexpr usize kMaxAtlasJsonBytes = 4u * 1024u * 1024u;
    static constexpr u32 kMaxJsonDepth = 64u;
    static constexpr usize kMaxJsonStringBytes = 4096u;
    static constexpr u32 kMaxJsonNodes = 100000u;
    static constexpr u32 kMaxJsonObjectMembers = 4096u;
    static constexpr u32 kMaxFrames = 4096u;
    static constexpr usize kMaxFrameNameBytes = 255u;
    static constexpr usize kMaxImagePathBytes = 1024u;
    static constexpr u32 kMaxAtlasDimension = 65535u;

    /** 空の atlas データを構築する (frame なし、メタ未設定)。 */
    FSpritePack() noexcept = default;

    /** 永続 string と frame array に呼び出し側所有の allocator を使う。 */
    explicit FSpritePack(IAllocator& allocator) noexcept
        : m_Frames(allocator),
          m_OwnedNames(allocator),
          m_OwnedImagePath(allocator) {}

    /** デストラクタ (frame 配列・所有文字列は TArray/FString が解放)。 */
    ~FSpritePack() noexcept = default;

    /** コピー禁止 (atlas データを不意に複製しないため)。 */
    FSpritePack(const FSpritePack&)            = delete;

    /** コピー代入も禁止。 */
    FSpritePack& operator=(const FSpritePack&) = delete;

    /** ムーブ禁止。 */
    FSpritePack(FSpritePack&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FSpritePack& operator=(FSpritePack&&)      = delete;

    /**
     * atlas メタ情報を設定する。
     *
     * @details
     * 値コピーで取り込み、既存の frame 配列はそのまま保持する (同じ atlas での再 Init 用途)。
     * frame もまとめてクリアしたい場合は ClearAll を併用する。
     * @param info 取り込む atlas メタ情報。
     */
    void Init(const FSpritePackInfo& info) noexcept;

    /**
     * frame を追加する。
     *
     * @details
     * name==nullptr は検索不能になるため無視する (debug ビルドでも crash させない)。
     * 同名 frame の重複登録は許容し、FindFrame は最初に一致したものを返す。
     * @param frame 追加する frame。
     */
    void AddFrame(const FSpriteFrame& frame) noexcept;

    /**
     * Aseprite / TexturePacker の sprite atlas JSON を読み込む。
     *
     * @details
     * frame 名と image path を内部で所有するため JSON テキストの寿命に依存しない。
     * 既存 frame は ClearAll してから読み込む。対応形式は Aseprite "hash"
     * ({ "frames": { "name": { "frame": {x,y,w,h}, ["pivot":{x,y}] } } }) と
     * TexturePacker "array" ({ "frames": [ { "filename": "name", "frame": {x,y,w,h} } ] })、
     * meta ({ "image": "...", "size": { "w":.., "h":.. } }) を atlas メタへ取り込む。
     * @param json_text atlas JSON のテキスト。
     * @param len json_text のバイト長。
     * @return 成功なら空の TResult、解析失敗 / frames 欠如なら ACS_ERR。
     */
    TResult<void> LoadAtlasJson(const char* json_text, usize len) noexcept;

    /**
     * atlas JSON を厳密に検証し、transactional に読み込む。
     *
     * 未知の exporter 拡張 member は許可する。JSON member・frame 名の重複、
     * 不正な矩形、非有限値は、既存 pack state や公開済み pointer を変更せず失敗する。
     */
    FSpritePackLoadResult TryLoadAtlasJson(
        const char* json_text, usize len) noexcept;

    /**
     * 名前で frame を検索する。
     *
     * @details 比較は pointer 同一 → strcmp の順。
     * @param name 検索するキー (nullptr は常に nullptr を返す)。
     * @return 見つかった frame へのポインタ (無ければ nullptr)。
     */
    const FSpriteFrame* FindFrame(const char* name) const noexcept;

    /**
     * 名前で frame の有無を確認する。
     *
     * @param name 検索するキー。
     * @return 存在すれば true (= FindFrame != nullptr)。
     */
    bool HasFrame(const char* name) const noexcept;

    /**
     * 登録 frame 数を返す。
     *
     * @return frame 数。
     */
    u32 FrameCount() const noexcept;

    /**
     * 内部 frame 配列の先頭ポインタを返す。
     *
     * @details ポインタは Init / AddFrame / RemoveFrame / ClearAll で無効化される。
     * @param out_count frame 数を書き戻す出力先。
     * @return frame 配列の先頭ポインタ。
     */
    const FSpriteFrame* AllFrames(u32& out_count) const noexcept;

    /**
     * atlas メタ情報を取得する。
     *
     * @return atlas メタ情報への const 参照。
     */
    const FSpritePackInfo& Info() const noexcept { return m_Info; }

    /**
     * 指定 name の frame を削除する。
     *
     * @details 複数一致しても全て削除する。順序は保持しない (末尾 swap)。
     * @param name 削除する frame の名前 (nullptr は no-op)。
     */
    void RemoveFrame(const char* name) noexcept;

    /** 全 frame を削除する (atlas メタ情報 m_Info は保持)。 */
    void ClearAll() noexcept;

    /**
     * frame の矩形を atlas size で割って [0,1] UV を計算する。
     *
     * @details atlas_width / atlas_height が 0 のときは 0 除算を避け {0,0,0,0} を返す。
     * @param frame UV を求める frame。
     * @return UV サブ矩形 {u0, v0, u1, v1}。
     */
    acs::FVec4 ComputeUv(const FSpriteFrame& frame) const noexcept;

private:
    /** atlas メタ情報。 */
    FSpritePackInfo     m_Info;

    /** 登録された frame の配列。 */
    TArray<FSpriteFrame> m_Frames;

    /** LoadAtlasJson が所有する frame 名バッファ (FSpriteFrame.name が指す、Reserve 済で安定)。 */
    TArray<FString>    m_OwnedNames;

    /** LoadAtlasJson が所有する atlas image path。 */
    FString            m_OwnedImagePath;
};

} // namespace acs::game
