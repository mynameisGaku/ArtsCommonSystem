// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — ParticleEditor `.fxedit` テキスト I/O (Phase 19b)
//
// 役割:
//   ParticleEditor (in-engine particle authoring tool) が編集中の emitter 群を
//   人間可読 / git diff 可能なテキスト形式で保存・復元するためのシリアライザ。
//   バイナリ形式の `FSaveSlot<T>` (Pillar J) と違い、**作業中アセットを版管理に
//   そのまま乗せられる** ことを最優先する (= UE5 の `.uasset` ではなく Unity の
//   `.meta` 風のフィロソフィー)。
//
// 使い方:
//   acs::game::ParticleEmitterDef defs[8] = {};
//   defs[0].lifetime_sec      = 2.0f;
//   defs[0].emit_rate_per_sec = 50.0f;
//   defs[0].color_start       = {1.0f, 0.8f, 0.2f};
//   defs[0].color_end         = {1.0f, 0.2f, 0.0f};
//   const char* names[]       = {"fire", "smoke"};
//
//   acs::game::fxedit::FFxeditSerializer::Save(L"data/effects/fireball.fxedit",
//                                              defs, names, 2);
//
//   // ロード側:
//   acs::game::ParticleEmitterDef loaded[16] = {};
//   char                          name_buf[16 * 32] = {};
//   auto r = acs::game::fxedit::FFxeditSerializer::Load(
//       L"data/effects/fireball.fxedit", loaded, name_buf, sizeof(name_buf), 16);
//   if (r.IsOk()) { u32 n = r.Value(); /* n 個ロード成功 */ }
//
// テキストフォーマット (`ACS_FXEDIT` v1):
//   ACS_FXEDIT 1
//   EMITTER count 2
//   E0 name "fire"
//   E0 emit_rate 50.0
//   E0 lifetime_sec 2.0
//   E0 burst_count 0
//   E0 speed_min 0.5
//   E0 speed_max 2.0
//   E0 scale_start 1.0
//   E0 scale_end 0.2
//   E0 gravity 0.0 -1.0 0.0
//   E0 color_start 1.0 0.8 0.2 1.0
//   E0 color_end 1.0 0.2 0.0 0.0
//   E0 spread_radians 3.14
//   E1 name "smoke"
//   ...
//
//   ・1 行 1 key=value、key 行頭は emitter index (E0, E1, ..., E<N-1>)。
//   ・数値は `%g` フォーマット (シリアライズ側) / `strtof` (デシリアライズ側)。
//   ・`name "..."` は二重引用符で囲んだ ASCII (現状エスケープ無し、簡素化)。
//   ・`#` 始まりはコメント (parse 時にスキップ)。
//   ・空行スキップ。
//   ・未知 key は無視 (前方互換: 将来 key を増やしても旧ローダで読める)。
//   ・実 `ParticleEmitterDef` 構造体は color が FVec3、gravity が FVec2 であり、
//     テキスト形式の 4 番目 (alpha) / 3 番目 (z) 成分はシリアライズ側で 1.0/0.0
//     を埋め、デシリアライズ側で破棄する。`spread_radians` は emitter def の
//     正式メンバではないため将来拡張用の予約 key として読み込みのみサポート
//     (現状は値を保持する場所が無いので捨てる)。
//
// 設計選択:
//   ・**Text + 1 行 = 1 key**: git diff で 1 パラメータ変更が 1 行 diff になる。
//   ・**emitter index prefix (E0, E1, ...)**: 順序が壊れても再構築可能、
//     かつ多重 emitter ファイル (1 ファイル = N emitters) を素直に扱える。
//   ・**Magic + Version**: 先頭行で `ACS_FXEDIT 1` を要求。Phase 2 以降 schema が
//     変わったら version をインクリメントし、後方互換ローダが分岐する。
//   ・**非コピー・非ムーブ static class**: state を持たないため。
//   ・**全 noexcept / STL 不使用 / TResult<T, FErrorCode>**: ACS 規約。
//   ・**file I/O は acs::FileSystem に委譲**: `<stdio.h>` 等の C 標準 I/O を
//     直接呼ばず、Win32 CreateFileW ベースの platform/FileSystem を使うことで
//     wchar_t パスや GetLastError 由来エラーが一貫して扱える。
//   ・**name buffer は呼び出し側持ち**: 内部に `TArray<char>` を持つ設計も
//     可能だが、ロード結果を ParticleEditor 側に流し込む際にコピーが必要に
//     なるため、最初から呼び出し側 buffer に書き込む方式にして余計な
//     allocation を省く。format は `name0\0name1\0name2\0...` 連結。
//
// 範囲外 (Phase 2+ で検討):
//   ・curve-based 補間 (color over lifetime 等) の serialize 形式。
//   ・テクスチャ参照 (path string) の serialize。
//   ・3D 化 (FVec3 gravity / FVec3 position 等)。
//   ・unicode emitter 名 (現状 ASCII printable のみ)。
//   ・エスケープシーケンス (現状 `"` を含む name は不正)。
//
// 注意:
//   ・本クラスは ParticleEmitterDef の型を不完全宣言 (forward decl) のみで
//     参照する (.h では `struct ParticleEmitterDef;`)。実体は .cpp 側で
//     `FParticleEffectSystem.h` を include する。これにより本ヘッダのインクルードコストを
//     最小に保つ。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// 前方宣言: 実体は `gameframework/FParticleEffectSystem.h`。
// .cpp 側で完全型を include する。
struct ParticleEmitterDef;

namespace fxedit {

// =============================================================================
// FFxeditSerializer — `.fxedit` テキストファイルの save/load
// -----------------------------------------------------------------------------
// すべてのメンバは static。state を持たない utility class なので、コンストラクタ
// / コピー / ムーブを禁止しておく (誤って実体化されるのを防ぐ)。
// =============================================================================
class FFxeditSerializer {
public:
    FFxeditSerializer()                                   = delete;
    ~FFxeditSerializer()                                  = delete;
    FFxeditSerializer(const FFxeditSerializer&)            = delete;
    FFxeditSerializer& operator=(const FFxeditSerializer&) = delete;
    FFxeditSerializer(FFxeditSerializer&&)                 = delete;
    FFxeditSerializer& operator=(FFxeditSerializer&&)      = delete;

    // ---- file format 定数 -----------------------------------------------
    // 先頭行に必ず付ける magic 文字列 + 現在のフォーマットバージョン。
    static constexpr const char* kMagic            = "ACS_FXEDIT";
    static constexpr u32         kCurrentVersion   = 1u;

    // emitter 1 個分のテキスト出力に必要な最大バイト数 (sprintf 用上限見積)。
    // 約 12 key * (prefix "E999 " 6B + name 32B + value 64B + newline) ≒ 1.5 KB。
    // 安全側で 2048B にしておく。
    static constexpr usize       kMaxBytesPerEmitter = 2048;

    // 1 行のテキストパース時に許容する最大長 (これ以上は切り捨てる)。
    static constexpr usize       kMaxLineLength      = 512;

    // 1 emitter の name に許容する最大バイト数 (NUL 終端含まず)。
    // ParticleEditor 側の UI 想定で 31 文字 (+NUL = 32B) としておく。
    static constexpr usize       kMaxEmitterName     = 31;

    // ---- FErrorCode subcode 定義 (ErrCategory::IO) -----------------------
    // FSaveSlot (1-99) と衝突しないよう、fxedit は 700-799 番を使う。
    enum SubCode : u16 {
        kSub_OK                = 0,
        kSub_NullArgs          = 700,  // file_path / defs / names が nullptr
        kSub_TooManyEmitters   = 701,  // count > max_emitters
        kSub_BufferOverflow    = 702,  // name buffer 不足 or 出力 buffer 不足
        kSub_BadMagic          = 703,  // 先頭が "ACS_FXEDIT" でない
        kSub_BadVersion        = 704,  // version が未対応
        kSub_BadFormat         = 705,  // 行の構文不正
        kSub_FileNotFound      = 706,  // 読み込み対象が存在しない
        kSub_IOFailure         = 707,  // 下位 FileSystem からのエラー
    };

    // -----------------------------------------------------------------------
    // Save — emitter 群を `.fxedit` テキストへ書き出す
    // -----------------------------------------------------------------------
    // file_path: 保存先 (Win32 wide path)。既存ファイルは上書き。
    // defs:      ParticleEmitterDef 配列 (count 個)。nullptr 不可。
    // names:     emitter 名 (C 文字列) の配列。nullptr 個別要素は "" 扱い。
    //            ただし names ポインタ自体が nullptr の場合は kSub_NullArgs。
    // count:     emitter 個数 (0 も valid: ヘッダだけ書き出す)。
    static TResult<void, FErrorCode> Save(const wchar_t*             file_path,
                                        const ParticleEmitterDef*  defs,
                                        const char* const*         names,
                                        u32                        count) noexcept;

    // -----------------------------------------------------------------------
    // Load — `.fxedit` テキストから emitter 群を読み出す
    // -----------------------------------------------------------------------
    // file_path:             読み込み元 (Win32 wide path)。
    // out_defs:              復元する ParticleEmitterDef 配列 (max_emitters 個分の領域)。
    // out_name_buffer:       名前バッファ (各 emitter の C string を連結配置)。
    //                        `name0\0name1\0...\0name<n-1>\0` 形式で書き込む。
    // name_buffer_capacity:  out_name_buffer のバイト数。
    // max_emitters:          out_defs の要素数 (= 受け入れ可能な最大 emitter 数)。
    // 戻り値: 成功時は実際にロードした emitter 数 (<= max_emitters)。
    static TResult<u32, FErrorCode> Load(const wchar_t*       file_path,
                                       ParticleEmitterDef*  out_defs,
                                       char*                out_name_buffer,
                                       u32                  name_buffer_capacity,
                                       u32                  max_emitters) noexcept;

    // -----------------------------------------------------------------------
    // ParseHeaderVersion — 先頭行の "ACS_FXEDIT <v>" を検査して v を返す
    // -----------------------------------------------------------------------
    // 0 を返した場合は magic mismatch / version 解析失敗を意味する。
    // text は NUL 終端でなくても OK (text_len で長さを与える)。
    static u32 ParseHeaderVersion(const char* text, u32 text_len) noexcept;

    // -----------------------------------------------------------------------
    // ParseLine — 1 行の "<key> <v0> [<v1> [<v2> [<v3>]]]" を解釈する
    // -----------------------------------------------------------------------
    // line は NUL 終端を期待 (ReadAllText が末尾 NUL 付きで読むため)。
    // out_key: key 文字列の **開始ポインタ** (line 内、終端 NUL 化はしない)。
    //          代わりに in-place NUL 化はせず、呼び出し側が長さを別途判定する。
    //          ここでは「最初の空白文字までを key 範囲」とする規約。
    // out_v0..v3: 数値スロット。出現しない値には 0.0 が入る。
    // 返り値: 構文が正しければ true。完全に空行 (key も無い) なら false。
    static bool ParseLine(const char*  line,
                          const char*& out_key,
                          f32&         out_v0,
                          f32&         out_v1,
                          f32&         out_v2,
                          f32&         out_v3) noexcept;

    // -----------------------------------------------------------------------
    // SkipWhitespace — ' ' / '\t' / '\r' / '\n' を読み飛ばす
    // -----------------------------------------------------------------------
    // 末端 NUL は跨がない (NUL に当たったらそこで止まる)。
    static const char* SkipWhitespace(const char* p) noexcept;
};

} // namespace fxedit
} // namespace acs::game
