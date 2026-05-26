// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar G / Q — FSpritePack (sprite atlas data + 名前付き frame lookup)
//
// 「1 枚の atlas テクスチャ + その中に並ぶ複数 frame の矩形」を持つだけのデータ層。
// 描画 API・asset loader には触れない。利用者は AtlasTexturePath() で texture を
// 自前ロードし、FindFrame("Idle_03") で矩形と pivot を取り出し、ComputeUv で
// [0, 1] UV を得る。FSpriteAnimator (時間→index) と組み合わせて animation を再生する。
//
// 使い方:
//   FSpritePack pack;
//   FSpritePackInfo info;
//   info.atlas_texture_path = "assets/hero_atlas.png";
//   info.atlas_width        = 1024;
//   info.atlas_height       = 1024;
//   pack.Init(info);
//
//   FSpriteFrame f;
//   f.name = "Idle_00";
//   f.x = 0;   f.y = 0;   f.w = 64; f.h = 64;
//   f.pivot_x = 0.5f; f.pivot_y = 0.5f;
//   pack.AddFrame(f);
//   // ...
//
//   if (const FSpriteFrame* frame = pack.FindFrame("Idle_00"); frame != nullptr) {
//       acs::FVec4 uv = pack.ComputeUv(*frame); // {u0, v0, u1, v1}
//       // → DrawSprite(uv, ...)
//   }
//
// 設計判断:
//   ・Pillar G (asset/IO の data layout) と Pillar Q (視覚世界, atlas) の交差点に
//     位置するモジュール。テクスチャの所有 / ロードは責務外 (Pillar G の FAssetBundle
//     / AssetPack 側) で、FSpritePack は「矩形と名前の辞書」に徹する。
//   ・name は `const char*` 借用 (caller 所有 = 文字列リテラルまたは別所有の
//     永続バッファ前提)。`<string>` 禁止に従い ACS 規約と整合。比較は pointer
//     同一 → strcmp の順で評価し、リテラル運用なら pointer 一致の高速 path を抜ける。
//   ・非コピー・非ムーブ: frame 配列を不意に複製しないため。所有権を明示したい
//     場合は Init/AddFrame を再呼出しすることで上書き or 別 instance を作る。
//   ・ComputeUv は atlas_width/atlas_height が 0 の場合に 0 除算を避け {0,0,0,0}
//     を返す。FSpriteAnimator と同様、不正状態でも crash しないことを優先。
//   ・RemoveFrame は順序非保持の swap remove (= 内部 TArray::RemoveAtSwap 相当)。
//     animation はインデックスではなく名前で参照する前提なので順序破壊は許容。
#pragma once

#include "foundation/Types.h"
#include "container/Array.h"
#include "math/Vec.h"

namespace acs::game {

// atlas 内の 1 矩形 = 1 frame。name は caller 所有 (文字列リテラル前提)。
// pivot は frame のローカル空間 [0,1] (0,0 = 左上 / 1,1 = 右下) で表現し、
// 描画時のアンカー (回転中心 / 配置基準) として使う。
struct FSpriteFrame {
    const char* name    = nullptr;  // 検索キー (caller 所有、リテラル想定)
    u32         x       = 0;        // atlas 内 X (pixel)
    u32         y       = 0;        // atlas 内 Y (pixel)
    u32         w       = 0;        // 幅 (pixel)
    u32         h       = 0;        // 高さ (pixel)
    f32         pivot_x = 0.5f;     // frame ローカル [0,1]、既定は中心
    f32         pivot_y = 0.5f;     // frame ローカル [0,1]、既定は中心
};

// atlas 全体のメタ情報。texture そのものは別モジュール (FAssetBundle 等) が所有。
struct FSpritePackInfo {
    const char* atlas_texture_path = nullptr;  // caller 所有
    u32         atlas_width        = 0;        // pixel (0 = 未設定)
    u32         atlas_height       = 0;        // pixel (0 = 未設定)
};

class FSpritePack {
public:
    FSpritePack() noexcept = default;
    ~FSpritePack() noexcept = default;

    // ACS 規約: atlas データを不意に複製しないよう非コピー・非ムーブ
    FSpritePack(const FSpritePack&)            = delete;
    FSpritePack& operator=(const FSpritePack&) = delete;
    FSpritePack(FSpritePack&&)                 = delete;
    FSpritePack& operator=(FSpritePack&&)      = delete;

    // atlas メタ情報を設定。既存の frame 配列はそのまま保持される
    // (= 同じ atlas で再 Init するユースケース)。frame もまとめてクリアしたい場合は
    // ClearAll を併用する。
    void Init(const FSpritePackInfo& info) noexcept;

    // frame を追加。name == nullptr は無視 (= debug ビルドでも crash させない)。
    // 同名 frame が既に存在しても重複登録は許容する (= 利用者の責任で重複を避ける、
    // FindFrame は最初に一致したものを返す)。
    void AddFrame(const FSpriteFrame& frame) noexcept;

    // 名前で frame を検索。見つからなければ nullptr。
    // 比較は pointer 同一 → strcmp の順。name == nullptr は常に nullptr。
    const FSpriteFrame* FindFrame(const char* name) const noexcept;

    // 名前で frame の有無を確認 (= FindFrame != nullptr の syntactic sugar)。
    bool HasFrame(const char* name) const noexcept;

    // 登録 frame 数。
    u32 FrameCount() const noexcept;

    // 内部配列の先頭ポインタを返す。out_count に frame 数を書き戻す。
    // ポインタは Init / AddFrame / RemoveFrame / ClearAll で無効化される。
    const FSpriteFrame* AllFrames(u32& out_count) const noexcept;

    // atlas メタ情報を取得。
    const FSpritePackInfo& Info() const noexcept { return _info; }

    // 指定 name の frame を削除。複数一致しても全て削除する。
    // 順序は保持しない (= 末尾 swap)。name == nullptr は no-op。
    void RemoveFrame(const char* name) noexcept;

    // 全 frame を削除。atlas メタ情報 (_info) は保持される。
    void ClearAll() noexcept;

    // UV 計算ヘルパ: frame の矩形を atlas size で割って [0,1] UV を返す。
    // 戻り値は {u0, v0, u1, v1}。atlas_width / atlas_height が 0 のときは
    // 0 除算を避け {0, 0, 0, 0} を返す (= 不正状態でも crash させない)。
    acs::FVec4 ComputeUv(const FSpriteFrame& frame) const noexcept;

private:
    FSpritePackInfo     _info;
    TArray<FSpriteFrame> _frames;
};

} // namespace acs::game
