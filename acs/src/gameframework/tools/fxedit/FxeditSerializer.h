// SPDX-License-Identifier: Apache-2.0
// GameFramework Tools — ParticleEditor `.fxedit` テキスト I/O
//
// 役割:
//   ParticleEditor (in-engine particle authoring tool) が編集中の emitter 群を
//   人間可読 / git diff 可能なテキスト形式で保存・復元するためのシリアライザ。
//   バイナリ形式の `TSaveSlot<T>` (Pillar J) と違い、**作業中アセットを版管理に
//   そのまま乗せられる** ことを最優先する (= UE5 の `.uasset` ではなく Unity の
//   `.meta` 風のフィロソフィー)。
//
// 使い方:
//   acs::game::FParticleEmitterDef defs[8] = {};
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
//   acs::game::FParticleEmitterDef loaded[16] = {};
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
//   ・実 `FParticleEmitterDef` 構造体は color が FVec3、gravity が FVec2 であり、
//     テキスト形式の 4 番目 (alpha) / 3 番目 (z) 成分はシリアライズ側で 1.0/0.0
//     を埋め、デシリアライズ側で破棄する。`spread_radians` は emitter def の
//     正式メンバではないため将来拡張用の予約 key として読み込みのみサポート
//     (現状は値を保持する場所が無いので捨てる)。
//
// 設計選択:
//   ・**Text + 1 行 = 1 key**: git diff で 1 パラメータ変更が 1 行 diff になる。
//   ・**emitter index prefix (E0, E1, ...)**: 順序が壊れても再構築可能、
//     かつ多重 emitter ファイル (1 ファイル = N emitters) を素直に扱える。
//   ・**Magic + Version**: 先頭行で `ACS_FXEDIT 1` を要求。schema が変わったら
//     version をインクリメントし、後方互換ローダが分岐する。
//   ・**非コピー・非ムーブ static class**: state を持たないため。
//   ・**全 noexcept / STL 不使用 / TResult<T, FErrorCode>**: ACS 規約。
//   ・**file I/O は acs::FFileSystem に委譲**: `<stdio.h>` 等の C 標準 I/O を
//     直接呼ばず、Win32 CreateFileW ベースの platform/FileSystem を使うことで
//     wchar_t パスや GetLastError 由来エラーが一貫して扱える。
//   ・**name buffer は呼び出し側持ち**: 内部に `TArray<char>` を持つ設計も
//     可能だが、ロード結果を ParticleEditor 側に流し込む際にコピーが必要に
//     なるため、最初から呼び出し側 buffer に書き込む方式にして余計な
//     allocation を省く。format は `name0\0name1\0name2\0...` 連結。
//
// 注意:
//   ・本クラスは FParticleEmitterDef の型を不完全宣言 (forward decl) のみで
//     参照する (.h では `struct FParticleEmitterDef;`)。実体は .cpp 側で
//     `ParticleEffectSystem.h` を include する。これにより本ヘッダのインクルードコストを
//     最小に保つ。
#pragma once

#include "foundation/Result.h"
#include "foundation/Types.h"

namespace acs::game {

// 前方宣言: 実体は `gameframework/ParticleEffectSystem.h`。.cpp 側で完全型を include する。
struct FParticleEmitterDef;

namespace fxedit {

/** `.fxedit` の checked parse/load/save が返す安定したエラー種別。 */
enum class EFxeditSerializeError : u8 {
    None = 0,
    NullArgument,
    PathTooLong,
    InputTooLarge,
    EmbeddedNul,
    TooManyLines,
    LineTooLong,
    BadMagic,
    UnsupportedVersion,
    MissingEmitterCount,
    DuplicateEmitterCount,
    TooManyEmitters,
    BufferTooSmall,
    InvalidSyntax,
    InvalidEmitterIndex,
    DuplicateKey,
    InvalidValue,
    ValueOutOfRange,
    NameTooLong,
    InvalidName,
    TooManyCurves,
    TooManyKeyframes,
    AllocationFailure,
    FileNotFound,
    FileOpenFailed,
    FileSizeFailed,
    FileChanged,
    FileReadFailed,
    FileWriteFailed,
    FileFlushFailed,
    FileCloseFailed,
    AtomicReplaceFailed,
};

/** `.fxedit` の checked operation 結果。 */
struct FFxeditSerializeResult {
    EFxeditSerializeError error = EFxeditSerializeError::None;
    u32 line = 0u;
    u32 emitter_count = 0u;
    u64 bytes_processed = 0u;
    u32 os_error = 0u;

    bool Succeeded() const noexcept { return error == EFxeditSerializeError::None; }
    static const char* ErrorName(EFxeditSerializeError error) noexcept;
};

/**
 * `.fxedit` テキストファイルの save/load を担う state-less ユーティリティ。
 *
 * @details
 * すべてのメンバは static。state を持たない utility class なので、コンストラクタ・
 * コピー・ムーブを禁止しておく (誤って実体化されるのを防ぐ)。emitter 群を人間可読 /
 * git diff 可能なテキスト (`ACS_FXEDIT` v1) で書き出し・復元する。
 */
class FFxeditSerializer {
public:
    /** 構築禁止 (state を持たない static ユーティリティのため)。 */
    FFxeditSerializer()                                   = delete;

    /** 破棄禁止 (実体化しないため)。 */
    ~FFxeditSerializer()                                  = delete;

    /** コピー禁止。 */
    FFxeditSerializer(const FFxeditSerializer&)            = delete;

    /** コピー代入も禁止。 */
    FFxeditSerializer& operator=(const FFxeditSerializer&) = delete;

    /** ムーブ禁止。 */
    FFxeditSerializer(FFxeditSerializer&&)                 = delete;

    /** ムーブ代入も禁止。 */
    FFxeditSerializer& operator=(FFxeditSerializer&&)      = delete;

    /** 先頭行に必ず付ける magic 文字列。 */
    static constexpr const char* kMagic            = "ACS_FXEDIT";

    /** 現在のフォーマットバージョン。 */
    static constexpr u32         kCurrentVersion   = 1u;

    /**
     * emitter 1 個分のテキスト出力に必要な最大バイト数 (sprintf 用上限見積)。
     *
     * @details
     * 約 12 key * (prefix "E999 " 6B + name 32B + value 64B + newline) ≒ 1.5 KB。
     * 安全側で 2048B にしておく。
     */
    static constexpr usize       kMaxBytesPerEmitter = 2048;

    /** 1 行のテキストパース時に許容する最大長 (超過はエラー)。 */
    static constexpr usize       kMaxLineLength      = 512;

    /**
     * 1 emitter の name に許容する最大バイト数 (NUL 終端含まず)。
     *
     * @details ParticleEditor 側の UI 想定で 31 文字 (+NUL = 32B) としておく。
     */
    static constexpr usize       kMaxEmitterName     = 31;

    /** checked loader が受理する `.fxedit` 全体の最大 byte 数。 */
    static constexpr usize       kMaxInputBytes      = 8u * 1024u * 1024u;

    /** 1 ファイルに保持できる emitter の安全上限。 */
    static constexpr u32         kMaxEmitterCount    = 1024u;

    /** コメント・空行を含む入力行数の安全上限。 */
    static constexpr u32         kMaxLineCount       = 65536u;

    /** 将来予約の curve 数上限（emitter ごと）。 */
    static constexpr u32         kMaxCurvesPerEmitter = 16u;

    /** 将来予約の keyframe 数上限（curve ごと）。 */
    static constexpr u32         kMaxKeyframesPerCurve = 256u;

    /** Win32 wide path の終端 NUL を除く最大文字数。 */
    static constexpr usize       kMaxPathChars       = 1023u;

    /**
     * save/load のエラー subcode (FErrorCode の EErrCategory::IO で使う)。
     *
     * @details TSaveSlot (1-99) と衝突しないよう、fxedit は 700-799 番を使う。
     */
    enum ESubCode : u16 {
        /** エラーなし。 */
        kSub_OK                = 0,

        /** file_path / defs / names が nullptr。 */
        kSub_NullArgs          = 700,

        /** count > max_emitters。 */
        kSub_TooManyEmitters   = 701,

        /** name buffer 不足 or 出力 buffer 不足。 */
        kSub_BufferOverflow    = 702,

        /** 先頭が "ACS_FXEDIT" でない。 */
        kSub_BadMagic          = 703,

        /** version が未対応。 */
        kSub_BadVersion        = 704,

        /** 行の構文不正。 */
        kSub_BadFormat         = 705,

        /** 読み込み対象が存在しない。 */
        kSub_FileNotFound      = 706,

        /** 下位 FFileSystem からのエラー。 */
        kSub_IOFailure         = 707,

        /** checked parser の schema/value 検証失敗。 */
        kSub_ValidationFailed  = 708,

        /** allocation failure。 */
        kSub_AllocationFailure = 709,

        /** atomic replace に失敗。 */
        kSub_AtomicReplace     = 710,
    };

    /**
     * 長さ付き `.fxedit` text を全検証後に出力へ commit する。
     *
     * @details text は NUL 終端不要。失敗時は out_defs と out_name_buffer を変更しない。
     */
    static FFxeditSerializeResult TryParseText(
        const char* text,
        usize text_size,
        FParticleEmitterDef* out_defs,
        char* out_name_buffer,
        usize name_buffer_capacity,
        u32 max_emitters) noexcept;

    /**
     * `.fxedit` を上限付き完全 read し、TryParseText で復元する。
     *
     * @details サイズ変化、embedded NUL、OOM、close failure を成功扱いしない。
     */
    static FFxeditSerializeResult TryLoad(
        const wchar_t* file_path,
        FParticleEmitterDef* out_defs,
        char* out_name_buffer,
        usize name_buffer_capacity,
        u32 max_emitters) noexcept;

    /**
     * emitter 群を事前検証し、一意 temp へ durable write 後に atomic replace する。
     *
     * @details 失敗時は既存の保存先を変更せず、作成した temp を削除する。
     */
    static FFxeditSerializeResult TrySave(
        const wchar_t* file_path,
        const FParticleEmitterDef* defs,
        const char* const* names,
        u32 count) noexcept;

    /**
     * emitter 群を `.fxedit` テキストへ書き出す。
     *
     * @details count が 0 のときはヘッダだけ書き出す。checked atomic save を経由する。
     * @param file_path 保存先 (Win32 wide path)。
     * @param defs FParticleEmitterDef 配列 (count 個)。count>0 のとき nullptr 不可。
     * @param names emitter 名 (C 文字列) の配列。個別要素 nullptr は "" 扱い。
     *              count>0 で names ポインタ自体が nullptr の場合は kSub_NullArgs。
     * @param count emitter 個数 (0 も valid)。
     * @return 成功なら空の TResult、null 引数 / バッファ溢れ / I/O 失敗ならエラー。
     */
    static TResult<void, FErrorCode> Save(const wchar_t*             file_path,
                                        const FParticleEmitterDef*  defs,
                                        const char* const*         names,
                                        u32                        count) noexcept;

    /**
     * `.fxedit` テキストから emitter 群を読み出す。
     *
     * @param file_path 読み込み元 (Win32 wide path)。
     * @param out_defs 復元する FParticleEmitterDef 配列 (max_emitters 個分の領域)。
     * @param out_name_buffer 名前バッファ。各 emitter の C string を `name0\0name1\0...`
     *                        形式で連結配置する。
     * @param name_buffer_capacity out_name_buffer のバイト数。
     * @param max_emitters out_defs の要素数 (= 受け入れ可能な最大 emitter 数)。
     * @return 成功なら実際にロードした emitter 数 (<= max_emitters)、null 引数 /
     *         magic 不一致 / version 不一致 / 容量超過 / I/O 失敗ならエラー。
     */
    static TResult<u32, FErrorCode> Load(const wchar_t*       file_path,
                                       FParticleEmitterDef*  out_defs,
                                       char*                out_name_buffer,
                                       u32                  name_buffer_capacity,
                                       u32                  max_emitters) noexcept;

    /**
     * 先頭の非空行から "ACS_FXEDIT <v>" を検査して version を返す。
     *
     * @details text は NUL 終端でなくても OK (text_len で長さを与える)。
     * @param text パース対象のテキスト先頭。
     * @param text_len text の長さ (バイト)。
     * @return 解析できた version。magic 不一致 / version 解析失敗なら 0。
     */
    static u32 ParseHeaderVersion(const char* text, u32 text_len) noexcept;

    /**
     * 1 行の "<key> <v0> [<v1> [<v2> [<v3>]]]" を構文解析する。
     *
     * @details
     * line は NUL 終端を期待する (ReadAllText が末尾 NUL 付きで読むため)。out_key は
     * line 内の key 開始ポインタを指す (終端 NUL 化はしないので長さは呼び出し側が
     * 「最初の空白文字まで」で判定する)。出現しない値スロットには 0.0 が入る。
     * @param line パース対象の 1 行 (NUL 終端)。
     * @param out_key key 文字列の開始ポインタ (line 内)。
     * @param out_v0 1 番目の数値スロット。
     * @param out_v1 2 番目の数値スロット。
     * @param out_v2 3 番目の数値スロット。
     * @param out_v3 4 番目の数値スロット。
     * @return 構文が正しければ true、空行 / コメント行なら false。
     */
    static bool ParseLine(const char*  line,
                          const char*& out_key,
                          f32&         out_v0,
                          f32&         out_v1,
                          f32&         out_v2,
                          f32&         out_v3) noexcept;

    /**
     * ' ' / '\t' / '\r' / '\n' を読み飛ばす。
     *
     * @details 末端 NUL は跨がない (NUL に当たったらそこで止まる)。
     * @param p 走査開始ポインタ (nullptr 可)。
     * @return 最初の非空白文字 (または NUL) を指すポインタ。p が nullptr なら nullptr。
     */
    static const char* SkipWhitespace(const char* p) noexcept;
};

} // namespace fxedit
} // namespace acs::game
